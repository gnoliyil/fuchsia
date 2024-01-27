// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod open;
pub mod providers;
pub mod service;
pub use ::routing::error::RoutingError;
pub use open::*;

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
            },
            storage,
        },
    },
    ::routing::{
        capability_source::ComponentCapability, component_instance::ComponentInstanceInterface,
        error::AvailabilityRoutingError, mapper::NoopRouteMapper, route_capability,
        route_storage_and_backing_directory,
    },
    cm_moniker::{InstancedExtendedMoniker, InstancedRelativeMoniker},
    cm_rust::{ExposeDecl, UseDecl, UseStorageDecl},
    cm_util::channel,
    fidl::{endpoints::ServerEnd, epitaph::ChannelEpitaphExt},
    fidl_fuchsia_io as fio, fuchsia_zircon as zx,
    futures::lock::Mutex,
    moniker::RelativeMonikerBase,
    std::path::PathBuf,
    std::sync::Arc,
    tracing::{debug, info, warn},
};

pub type RouteRequest = ::routing::RouteRequest;
pub type RouteSource = ::routing::RouteSource<ComponentInstance>;

/// Routes a capability from `target` to its source. Opens the capability if routing succeeds.
///
/// If the capability is not allowed to be routed to the `target`, per the
/// [`crate::model::policy::GlobalPolicyChecker`], the capability is not opened and an error
/// is returned.
pub(super) async fn route_and_open_capability(
    route_request: RouteRequest,
    target: &Arc<ComponentInstance>,
    open_options: OpenOptions<'_>,
) -> Result<(), ModelError> {
    let route_source = match route_request {
        RouteRequest::UseStorage(use_storage_decl) => {
            route_storage_and_backing_directory(
                use_storage_decl,
                target,
                &mut NoopRouteMapper,
                &mut NoopRouteMapper,
            )
            .await?
        }
        _ => {
            let optional_use = route_request.target_use_optional();
            route_capability(route_request, target, &mut NoopRouteMapper).await.map_err(|err| {
                if optional_use {
                    match err {
                        RoutingError::AvailabilityRoutingError(_) => {
                            // `err` is already an AvailabilityRoutingError.
                            // Return it as-is.
                            err
                        }
                        _ => {
                            // Wrap the error, to surface the target's
                            // optional usage.
                            RoutingError::AvailabilityRoutingError(
                                AvailabilityRoutingError::FailedToRouteToOptionalTarget {
                                    reason: err.to_string(),
                                },
                            )
                        }
                    }
                } else {
                    // Not an optional `use` so return the error as-is.
                    err
                }
            })?
        }
    };
    OpenRequest::new_from_route_source(route_source, target, open_options).open().await
}

/// Routes a capability from `target` to its source.
///
/// If the capability is not allowed to be routed to the `target`, per the
/// [`crate::model::policy::GlobalPolicyChecker`], the capability is not opened and an error
/// is returned.
pub async fn route(
    route_request: RouteRequest,
    target: &Arc<ComponentInstance>,
) -> Result<(), RoutingError> {
    match route_request {
        RouteRequest::UseStorage(use_storage_decl) => {
            route_storage_and_backing_directory(
                use_storage_decl,
                target,
                &mut NoopRouteMapper,
                &mut NoopRouteMapper,
            )
            .await?;
        }
        _ => {
            route_capability(route_request, target, &mut NoopRouteMapper).await?;
        }
    }
    Ok(())
}

/// Routes a capability from `target` to its source, starting from a `use_decl`.
///
/// If the capability is allowed to be routed to the `target`, per the
/// [`crate::model::policy::GlobalPolicyChecker`], the capability is then opened at its source
/// triggering a `CapabilityRouted` event.
///
/// See [`fidl_fuchsia_io::Directory::Open`] for how the `flags`, `relative_path`,
/// and `server_chan` parameters are used in the open call.
///
/// Only capabilities that can be installed in a namespace are supported: Protocol, Service,
/// Directory, and Storage.
pub(super) async fn route_and_open_namespace_capability(
    flags: fio::OpenFlags,
    relative_path: String,
    use_decl: UseDecl,
    target: &Arc<ComponentInstance>,
    server_chan: &mut zx::Channel,
) -> Result<(), ModelError> {
    let route_request = request_for_namespace_capability_use(use_decl)?;
    let open_options =
        OpenOptions::for_namespace_capability(&route_request, flags, relative_path, server_chan)?;
    route_and_open_capability(route_request, target, open_options).await
}

/// Routes a capability from `target` to its source, starting from an `expose_decl`.
///
/// If the capability is allowed to be routed to the `target`, per the
/// [`crate::model::policy::GlobalPolicyChecker`], the capability is then opened at its source
/// triggering a `CapabilityRouted` event.
///
/// See [`fidl_fuchsia_io::Directory::Open`] for how the `flags`, `relative_path`,
/// and `server_chan` parameters are used in the open call.
///
/// Only capabilities that can both be opened from a VFS and be exposed to their parent
/// are supported: Protocol, Service, and Directory.
pub(super) async fn route_and_open_namespace_capability_from_expose(
    flags: fio::OpenFlags,
    relative_path: String,
    expose_decl: ExposeDecl,
    target: &Arc<ComponentInstance>,
    server_chan: &mut zx::Channel,
) -> Result<(), ModelError> {
    let route_request = request_for_namespace_capability_expose(expose_decl)?;
    let open_options =
        OpenOptions::for_namespace_capability(&route_request, flags, relative_path, server_chan)?;
    route_and_open_capability(route_request, target, open_options).await
}

/// Create a new `RouteRequest` from a `UseDecl`, checking that the capability type can
/// be installed in a namespace.
pub fn request_for_namespace_capability_use(use_decl: UseDecl) -> Result<RouteRequest, ModelError> {
    match use_decl {
        UseDecl::Directory(decl) => Ok(RouteRequest::UseDirectory(decl)),
        UseDecl::Protocol(decl) => Ok(RouteRequest::UseProtocol(decl)),
        UseDecl::Service(decl) => Ok(RouteRequest::UseService(decl)),
        UseDecl::Storage(decl) => Ok(RouteRequest::UseStorage(decl)),
        _ => Err(ModelError::unsupported("capability cannot be installed in a namespace")),
    }
}

/// Create a new `RouteRequest` from an `ExposeDecl`, checking that the capability type can
/// be installed in a namespace.
pub fn request_for_namespace_capability_expose(
    expose_decl: ExposeDecl,
) -> Result<RouteRequest, ModelError> {
    match expose_decl {
        ExposeDecl::Directory(decl) => Ok(RouteRequest::ExposeDirectory(decl)),
        ExposeDecl::Protocol(decl) => Ok(RouteRequest::ExposeProtocol(decl)),
        ExposeDecl::Service(decl) => Ok(RouteRequest::ExposeService(decl)),
        _ => Err(ModelError::unsupported("capability cannot be installed in a namespace")),
    }
}

/// Returns an instance of the default capability provider for the capability at `source`, if supported.
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
            Some(path) => Ok(Some(Box::new(NamespaceCapabilityProvider { path: path.clone() }))),
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
                    let base_capability_provider = Box::new(DefaultComponentCapabilityProvider {
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
        CapabilitySource::Aggregate { capability_provider, component, .. } => Ok(Some(Box::new(
            AggregateServiceDirectoryProvider::create(
                component.clone(),
                target,
                capability_provider.clone(),
            )
            .await?,
        ))),
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

/// Implementation detail of `OpenRequest::open`.
pub(super) async fn open_capability_at_source(
    flags: fio::OpenFlags,
    relative_path: PathBuf,
    source: CapabilitySource,
    target: &Arc<ComponentInstance>,
    server_chan: &mut zx::Channel,
) -> Result<(), ModelError> {
    let capability_provider =
        if let Some(provider) = get_default_provider(target.as_weak(), &source).await? {
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
        capability_provider.open(task_scope, flags, relative_path, server_chan).await?;
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
                return Err(ModelError::unsupported("filtered service"));
            }
            CapabilitySource::Framework { capability, component } => {
                return Err(RoutingError::capability_from_framework_not_found(
                    &component.abs_moniker,
                    capability.source_name().to_string(),
                )
                .into());
            }
            CapabilitySource::Capability { source_capability, component } => {
                return Err(RoutingError::capability_from_capability_not_found(
                    &component.abs_moniker,
                    source_capability.to_string(),
                )
                .into());
            }
            CapabilitySource::Builtin { capability, .. } => {
                return Err(ModelError::from(
                    RoutingError::capability_from_component_manager_not_found(
                        capability.source_name().to_string(),
                    ),
                ));
            }
            CapabilitySource::Namespace { capability, .. } => {
                return Err(ModelError::from(
                    RoutingError::capability_from_component_manager_not_found(
                        capability.source_id(),
                    ),
                ));
            }
            CapabilitySource::Collection { .. } => {
                return Err(ModelError::unsupported("collections"));
            }
            CapabilitySource::Aggregate { .. } => {
                return Err(ModelError::unsupported("aggregate capability"));
            }
        };
    }
}

/// Routes a storage capability from `target` to its source and deletes its isolated storage.
pub(super) async fn route_and_delete_storage(
    use_storage_decl: UseStorageDecl,
    target: &Arc<ComponentInstance>,
) -> Result<(), ModelError> {
    match route_storage_and_backing_directory(
        use_storage_decl,
        target,
        &mut NoopRouteMapper,
        &mut NoopRouteMapper,
    )
    .await?
    {
        RouteSource::StorageBackingDirectory(storage_source_info) => {
            // As of today, the storage component instance must contain the target. This is because
            // it is impossible to expose storage declarations up.
            let relative_moniker = InstancedRelativeMoniker::scope_down(
                &storage_source_info.storage_source_moniker,
                &target.instanced_moniker(),
            )
            .unwrap();
            storage::delete_isolated_storage(
                storage_source_info,
                target.persistent_storage,
                relative_moniker,
                target.instance_id().as_ref(),
            )
            .await
        }
        _ => unreachable!("impossible route source"),
    }
}

static ROUTE_ERROR_HELP: &'static str = "To learn more, see \
https://fuchsia.dev/go/components/connect-errors";

/// Sets an epitaph on `server_end` for a capability routing failure, and logs the error. Logs a
/// failure to route a capability. Formats `err` as a `String`, but elides the type if the error is
/// a `RoutingError`, the common case.
pub async fn report_routing_failure(
    target: &Arc<ComponentInstance>,
    cap: &ComponentCapability,
    err: &ModelError,
    server_end: zx::Channel,
) {
    server_end
        .close_with_epitaph(err.as_zx_status())
        .unwrap_or_else(|error| debug!(%error, "failed to send epitaph"));
    let err_str = match err {
        ModelError::RoutingError { err } => err.to_string(),
        _ => err.to_string(),
    };
    target
        .with_logger_as_default(|| {
            match err {
                ModelError::RoutingError {
                    err:
                        RoutingError::AvailabilityRoutingError(
                            AvailabilityRoutingError::OfferFromVoidToOptionalTarget,
                        ),
                } => {
                    // If the route failed because the capability is
                    // intentionally not provided, then this failure is expected
                    // and the warn level is unwarranted, so use the debug level
                    // in this case.
                    debug!(
                        "Optional {} `{}` was not available for target component `{}`: {}\n{}",
                        cap.type_name(),
                        cap.source_id(),
                        &target.abs_moniker,
                        &err_str,
                        ROUTE_ERROR_HELP
                    );
                }
                ModelError::RoutingError {
                    err:
                        RoutingError::AvailabilityRoutingError(
                            AvailabilityRoutingError::FailedToRouteToOptionalTarget { .. },
                        ),
                } => {
                    // If the target declared the capability as optional, but
                    // the capability could not be routed (such as if the source
                    // component is not available) the component _should_
                    // tolerate the missing optional capability. However, this
                    // should be logged. Developers are encouraged to change how
                    // they build and/or assemble different product
                    // configurations so declared routes are always end-to-end
                    // complete routes.
                    // TODO(fxbug.dev/109112): if we change the log for
                    // `Required` capabilities to `error!()`, consider also
                    // changing this log for `Optional` to `warn!()`.
                    info!(
                        "Optional {} `{}` was not available for target component `{}`: {}\n{}",
                        cap.type_name(),
                        cap.source_id(),
                        &target.abs_moniker,
                        &err_str,
                        ROUTE_ERROR_HELP
                    );
                }
                _ => {
                    // TODO(fxbug.dev/109112): consider changing this to `error!()`
                    warn!(
                        "Required {} `{}` was not available for target component `{}`: {}\n{}",
                        cap.type_name(),
                        cap.source_id(),
                        &target.abs_moniker,
                        &err_str,
                        ROUTE_ERROR_HELP
                    );
                }
            }
        })
        .await
}

/// Implementation detail of `OpenRequest::open`.
pub(super) async fn open_storage_capability(
    flags: fio::OpenFlags,
    relative_path: String,
    source: &storage::StorageCapabilitySource,
    target: &Arc<ComponentInstance>,
    server_chan: &mut zx::Channel,
) -> Result<(), ModelError> {
    // As of today, the storage component instance must contain the target. This is because it is
    // impossible to expose storage declarations up.
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
    let server_chan = channel::take_channel(server_chan);

    // If there is no relative path, assume it is the current directory. We use "."
    // because `fuchsia.io/Directory.Open` does not accept empty paths.
    let relative_path = if relative_path.is_empty() { "." } else { &relative_path };

    storage_dir_proxy
        .open(flags, fio::ModeType::empty(), relative_path, ServerEnd::new(server_chan))
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
    return Ok(());
}
