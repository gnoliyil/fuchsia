// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        capability::{CapabilityProvider, CapabilitySource},
        model::{
            component::{ComponentInstance, ExtendedInstance, StartReason, WeakComponentInstance},
            error::ModelError,
            hooks::{Event, EventPayload},
            routing::{
                providers::{
                    DefaultComponentCapabilityProvider, DirectoryEntryCapabilityProvider,
                    NamespaceCapabilityProvider,
                },
                service::{
                    AggregateServiceDirectoryProvider, CollectionServiceDirectory,
                    CollectionServiceRoute, FilteredServiceProvider,
                },
                RouteSource, RoutingError,
            },
            storage::{self, BackingDirectoryInfo},
        },
    },
    ::routing::{component_instance::ComponentInstanceInterface, path::PathBufExt},
    cm_moniker::{InstancedExtendedMoniker, InstancedRelativeMoniker},
    cm_util::channel,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io as fio, fuchsia_zircon as zx,
    futures::lock::Mutex,
    moniker::RelativeMonikerBase,
    std::{path::PathBuf, sync::Arc},
};

/// Contains the options to use when opening a capability.
///
/// See [`fidl_fuchsia_io::Directory::Open`] for how the `flags`, `relative_path`,
/// and `server_chan` parameters are used in the open call.
pub struct OpenOptions<'a> {
    pub flags: fio::OpenFlags,
    pub relative_path: String,
    pub server_chan: &'a mut zx::Channel,
}

/// A request to open a capability at its source.
pub enum OpenRequest<'a> {
    // Open a capability backed by a component's outgoing directory.
    OutgoingDirectory {
        open_options: OpenOptions<'a>,
        source: CapabilitySource,
        target: &'a Arc<ComponentInstance>,
    },
    // Open a storage capability.
    Storage {
        open_options: OpenOptions<'a>,
        source: storage::BackingDirectoryInfo,
        target: &'a Arc<ComponentInstance>,
    },
}

impl<'a> OpenRequest<'a> {
    /// Creates a request to open a capability with source `route_source` for `target`.
    pub fn new_from_route_source(
        route_source: RouteSource,
        target: &'a Arc<ComponentInstance>,
        mut open_options: OpenOptions<'a>,
    ) -> Self {
        let RouteSource { source, relative_path } = route_source;
        open_options.relative_path =
            relative_path.attach(open_options.relative_path).to_string_lossy().into();
        Self::OutgoingDirectory { open_options, source, target }
    }

    /// Creates a request to open a storage capability with source `storage_source` for `target`.
    pub fn new_from_storage_source(
        source: BackingDirectoryInfo,
        target: &'a Arc<ComponentInstance>,
        open_options: OpenOptions<'a>,
    ) -> Self {
        Self::Storage { open_options, source, target }
    }

    /// Opens the capability in `self`, triggering a `CapabilityRouted` event and binding
    /// to the source component instance if necessary.
    pub async fn open(self) -> Result<(), ModelError> {
        match self {
            Self::OutgoingDirectory { open_options, source, target } => {
                Self::open_outgoing_directory(open_options, source, target).await
            }
            Self::Storage { open_options, source, target } => {
                Self::open_storage(open_options, &source, target).await
            }
        }
    }

    async fn open_outgoing_directory(
        mut open_options: OpenOptions<'a>,
        source: CapabilitySource,
        target: &Arc<ComponentInstance>,
    ) -> Result<(), ModelError> {
        let capability_provider =
            if let Some(provider) = Self::get_default_provider(target.as_weak(), &source).await? {
                Some(provider)
            } else {
                // Dispatch a CapabilityRouted event to get a capability provider
                let mutexed_provider = Arc::new(Mutex::new(None));

                let event = Event::new(
                    &target,
                    EventPayload::CapabilityRouted {
                        source: source.clone(),
                        capability_provider: mutexed_provider.clone(),
                    },
                );

                // Get a capability provider from the tree
                target.hooks.dispatch(&event).await;

                let provider = mutexed_provider.lock().await.take();
                provider
            };

        if let Some(capability_provider) = capability_provider {
            let source_instance = source.source_instance().upgrade()?;
            let task_scope = match source_instance {
                ExtendedInstance::AboveRoot(top) => top.task_scope(),
                ExtendedInstance::Component(component) => component.nonblocking_task_scope(),
            };
            capability_provider
                .open(
                    task_scope,
                    open_options.flags,
                    PathBuf::from(open_options.relative_path),
                    &mut open_options.server_chan,
                )
                .await?;
            Ok(())
        } else {
            match &source {
                CapabilitySource::Component { .. } => {
                    unreachable!(
                        "Capability source is a component, which should have been caught by \
                    default_capability_provider: {:?}",
                        source
                    );
                }
                CapabilitySource::FilteredService { .. } => {
                    Err(ModelError::unsupported("filtered service"))
                }
                CapabilitySource::Framework { capability, component } => {
                    Err(RoutingError::capability_from_framework_not_found(
                        &component.abs_moniker,
                        capability.source_name().to_string(),
                    )
                    .into())
                }
                CapabilitySource::Capability { source_capability, component } => {
                    Err(RoutingError::capability_from_capability_not_found(
                        &component.abs_moniker,
                        source_capability.to_string(),
                    )
                    .into())
                }
                CapabilitySource::Builtin { capability, .. } => Err(ModelError::from(
                    RoutingError::capability_from_component_manager_not_found(
                        capability.source_name().to_string(),
                    ),
                )),
                CapabilitySource::Namespace { capability, .. } => Err(ModelError::from(
                    RoutingError::capability_from_component_manager_not_found(
                        capability.source_id(),
                    ),
                )),
                CapabilitySource::Collection { .. } => Err(ModelError::unsupported("collections")),
                CapabilitySource::Aggregate { .. } => {
                    Err(ModelError::unsupported("aggregate capability"))
                }
            }
        }
    }

    async fn open_storage(
        open_options: OpenOptions<'a>,
        source: &storage::BackingDirectoryInfo,
        target: &Arc<ComponentInstance>,
    ) -> Result<(), ModelError> {
        // As of today, the storage component instance must contain the target. This is because it
        // is impossible to expose storage declarations up.
        let relative_moniker = InstancedRelativeMoniker::scope_down(
            &source.storage_source_moniker,
            &target.instanced_moniker(),
        )
        .unwrap();

        let dir_source = source.storage_provider.clone();
        let storage_dir_proxy = storage::open_isolated_storage(
            &source,
            target.persistent_storage,
            relative_moniker.clone(),
            target.instance_id().as_ref(),
        )
        .await
        .map_err(|e| ModelError::from(e))?;

        // Open the storage with the provided flags, mode, relative_path and server_chan.
        // We don't clone the directory because we can't specify the mode or path that way.
        let server_chan = channel::take_channel(open_options.server_chan);

        // If there is no relative path, assume it is the current directory. We use "."
        // because `fuchsia.io/Directory.Open` does not accept empty paths.
        let relative_path =
            if open_options.relative_path.is_empty() { "." } else { &open_options.relative_path };

        storage_dir_proxy
            .open(
                open_options.flags,
                fio::ModeType::empty(),
                relative_path,
                ServerEnd::new(server_chan),
            )
            .map_err(|err| {
                let moniker = match &dir_source {
                    Some(r) => {
                        InstancedExtendedMoniker::ComponentInstance(r.instanced_moniker().clone())
                    }
                    None => InstancedExtendedMoniker::ComponentManager,
                };
                ModelError::OpenStorageFailed {
                    moniker,
                    relative_moniker,
                    path: relative_path.to_string(),
                    err,
                }
            })?;
        Ok(())
    }

    /// Returns an instance of the default capability provider for the capability at `source`, if
    /// supported.
    async fn get_default_provider(
        target: WeakComponentInstance,
        source: &CapabilitySource,
    ) -> Result<Option<Box<dyn CapabilityProvider>>, ModelError> {
        match source {
            CapabilitySource::Component { capability, component } => {
                // Route normally for a component capability with a source path
                Ok(match capability.source_path() {
                    Some(path) => Some(Box::new(DefaultComponentCapabilityProvider {
                        target,
                        source: component.clone(),
                        name: capability
                            .source_name()
                            .expect("capability with source path should have a name")
                            .clone(),
                        path: path.clone(),
                    })),
                    _ => None,
                })
            }
            CapabilitySource::Namespace { capability, .. } => match capability.source_path() {
                Some(path) => {
                    Ok(Some(Box::new(NamespaceCapabilityProvider { path: path.clone() })))
                }
                _ => Ok(None),
            },
            CapabilitySource::FilteredService {
                capability,
                component,
                source_instance_filter,
                instance_name_source_to_target,
            } => {
                // First get the base service capability provider
                match capability.source_path() {
                    Some(path) => {
                        let base_capability_provider =
                            Box::new(DefaultComponentCapabilityProvider {
                                target,
                                source: component.clone(),
                                name: capability
                                    .source_name()
                                    .expect("capability with source path should have a name")
                                    .clone(),
                                path: path.clone(),
                            });

                        let source_component = component.upgrade()?;
                        let provider = FilteredServiceProvider::new(
                            &source_component,
                            source_instance_filter.clone(),
                            instance_name_source_to_target.clone(),
                            base_capability_provider,
                        )
                        .await?;
                        Ok(Some(Box::new(provider)))
                    }
                    _ => Ok(None),
                }
            }
            CapabilitySource::Aggregate { capability_provider, component, .. } => {
                Ok(Some(Box::new(
                    AggregateServiceDirectoryProvider::create(
                        component.clone(),
                        target,
                        capability_provider.clone(),
                    )
                    .await?,
                )))
            }
            CapabilitySource::Collection {
                capability,
                component,
                aggregate_capability_provider,
                collection_name,
            } => {
                let source_component_instance = component.upgrade()?;

                let route = CollectionServiceRoute {
                    source_moniker: source_component_instance.abs_moniker.clone(),
                    collection_name: collection_name.clone(),
                    service_name: capability.source_name().clone(),
                };

                source_component_instance
                    .start(&StartReason::AccessCapability {
                        target: target.abs_moniker.clone(),
                        name: capability.source_name().clone(),
                    })
                    .await?;

                // If there is an existing collection service directory, provide it.
                {
                    let state = source_component_instance.lock_resolved_state().await?;
                    if let Some(service_dir) = state.collection_services.get(&route) {
                        let provider = DirectoryEntryCapabilityProvider {
                            execution_scope: state.execution_scope().clone(),
                            entry: service_dir.dir_entry().await,
                        };
                        return Ok(Some(Box::new(provider)));
                    }
                }

                // Otherwise, create one. This must be done while the component ResolvedInstanceState
                // is unlocked because the AggregateCapabilityProvider uses locked state.
                let service_dir = Arc::new(CollectionServiceDirectory::new(
                    component.clone(),
                    route.clone(),
                    aggregate_capability_provider.clone_boxed(),
                ));

                source_component_instance.hooks.install(service_dir.hooks()).await;

                let provider = {
                    let mut state = source_component_instance.lock_resolved_state().await?;
                    let execution_scope = state.execution_scope().clone();
                    let entry = service_dir.dir_entry().await;

                    state.collection_services.insert(route, service_dir.clone());

                    DirectoryEntryCapabilityProvider { execution_scope, entry }
                };

                // Populate the service dir with service entries from children that may have been started before the service
                // capability had been routed from the collection.
                service_dir.add_entries_from_children().await?;

                Ok(Some(Box::new(provider)))
            }
            CapabilitySource::Framework { .. }
            | CapabilitySource::Capability { .. }
            | CapabilitySource::Builtin { .. } => {
                // There is no default provider for a framework or builtin capability
                Ok(None)
            }
        }
    }
}
