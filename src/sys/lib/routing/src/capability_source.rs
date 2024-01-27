// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        component_instance::{
            ComponentInstanceInterface, WeakComponentInstanceInterface,
            WeakExtendedInstanceInterface,
        },
        error::RoutingError,
    },
    async_trait::async_trait,
    cm_rust::{
        CapabilityDecl, CapabilityTypeName, DirectoryDecl, EventStreamDecl, ExposeDecl,
        ExposeDirectoryDecl, ExposeProtocolDecl, ExposeResolverDecl, ExposeRunnerDecl,
        ExposeServiceDecl, ExposeSource, OfferDecl, OfferDirectoryDecl, OfferEventStreamDecl,
        OfferProtocolDecl, OfferResolverDecl, OfferRunnerDecl, OfferServiceDecl, OfferSource,
        OfferStorageDecl, ProtocolDecl, RegistrationSource, ResolverDecl, RunnerDecl, ServiceDecl,
        StorageDecl, UseDecl, UseDirectoryDecl, UseProtocolDecl, UseServiceDecl, UseSource,
        UseStorageDecl,
    },
    cm_types::{Name, Path},
    derivative::Derivative,
    from_enum::FromEnum,
    moniker::ChildMoniker,
    std::{collections::HashMap, fmt, sync::Weak},
    thiserror::Error,
};

#[derive(Debug, Error)]
pub enum Error {
    #[error("Invalid framework capability.")]
    InvalidFrameworkCapability {},
    #[error("Invalid builtin capability.")]
    InvalidBuiltinCapability {},
}

/// Describes the source of a capability, as determined by `find_capability_source`
#[derive(Debug, Derivative)]
#[derivative(Clone(bound = ""))]
pub enum CapabilitySource<C: ComponentInstanceInterface> {
    /// This capability originates from the component instance for the given Realm.
    /// point.
    Component { capability: ComponentCapability, component: WeakComponentInstanceInterface<C> },
    /// This capability originates from "framework". It's implemented by component manager and is
    /// scoped to the realm of the source.
    Framework { capability: InternalCapability, component: WeakComponentInstanceInterface<C> },
    /// This capability originates from the parent of the root component, and is built in to
    /// component manager. `top_instance` is the instance at the top of the tree, i.e.  the
    /// instance representing component manager.
    Builtin { capability: InternalCapability, top_instance: Weak<C::TopInstance> },
    /// This capability originates from the parent of the root component, and is offered from
    /// component manager's namespace. `top_instance` is the instance at the top of the tree, i.e.
    /// the instance representing component manager.
    Namespace { capability: ComponentCapability, top_instance: Weak<C::TopInstance> },
    /// This capability is provided by the framework based on some other capability.
    Capability {
        source_capability: ComponentCapability,
        component: WeakComponentInstanceInterface<C>,
    },
    /// This capability is an aggregate of capabilities provided by components in a collection.
    CollectionAggregate {
        capability: AggregateCapability,
        component: WeakComponentInstanceInterface<C>,
        aggregate_capability_provider: Box<dyn CollectionAggregateCapabilityProvider<C>>,
        collections: Vec<String>,
    },
    OfferAggregate {
        capability: AggregateCapability,
        capability_provider: Box<dyn OfferAggregateCapabilityProvider<C>>,
        component: WeakComponentInstanceInterface<C>,
    },
    FilteredService {
        capability: ComponentCapability,
        component: WeakComponentInstanceInterface<C>,
        source_instance_filter: Vec<String>,
        instance_name_source_to_target: HashMap<String, Vec<String>>,
    },
}

impl<C: ComponentInstanceInterface> CapabilitySource<C> {
    /// Returns whether the given CapabilitySourceInterface can be available in a component's
    /// namespace.
    pub fn can_be_in_namespace(&self) -> bool {
        match self {
            Self::Component { capability, .. } => capability.can_be_in_namespace(),
            Self::Framework { capability, .. } => capability.can_be_in_namespace(),
            Self::Builtin { capability, .. } => capability.can_be_in_namespace(),
            Self::Namespace { capability, .. } => capability.can_be_in_namespace(),
            Self::Capability { .. } => true,
            Self::CollectionAggregate { capability, .. } => capability.can_be_in_namespace(),
            Self::OfferAggregate { capability, .. } => capability.can_be_in_namespace(),
            Self::FilteredService { capability, .. } => capability.can_be_in_namespace(),
        }
    }

    pub fn source_name(&self) -> Option<&Name> {
        match self {
            Self::Component { capability, .. } => capability.source_name(),
            Self::Framework { capability, .. } => Some(capability.source_name()),
            Self::Builtin { capability, .. } => Some(capability.source_name()),
            Self::Namespace { capability, .. } => capability.source_name(),
            Self::Capability { .. } => None,
            Self::CollectionAggregate { capability, .. } => Some(capability.source_name()),
            Self::OfferAggregate { capability, .. } => Some(capability.source_name()),
            Self::FilteredService { capability, .. } => capability.source_name(),
        }
    }

    pub fn type_name(&self) -> CapabilityTypeName {
        match self {
            Self::Component { capability, .. } => capability.type_name(),
            Self::Framework { capability, .. } => capability.type_name(),
            Self::Builtin { capability, .. } => capability.type_name(),
            Self::Namespace { capability, .. } => capability.type_name(),
            Self::Capability { source_capability, .. } => source_capability.type_name(),
            Self::CollectionAggregate { capability, .. } => capability.type_name(),
            Self::OfferAggregate { capability, .. } => capability.type_name(),
            Self::FilteredService { capability, .. } => capability.type_name(),
        }
    }

    pub fn source_instance(&self) -> WeakExtendedInstanceInterface<C> {
        match self {
            Self::Component { component, .. }
            | Self::Framework { component, .. }
            | Self::Capability { component, .. }
            | Self::FilteredService { component, .. }
            | Self::CollectionAggregate { component, .. }
            | Self::OfferAggregate { component, .. } => {
                WeakExtendedInstanceInterface::Component(component.clone())
            }
            Self::Builtin { top_instance, .. } | Self::Namespace { top_instance, .. } => {
                WeakExtendedInstanceInterface::AboveRoot(top_instance.clone())
            }
        }
    }
}

impl<C: ComponentInstanceInterface> fmt::Display for CapabilitySource<C> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}",
            match self {
                CapabilitySource::Component { capability, component } => {
                    format!("{} '{}'", capability, component.abs_moniker)
                }
                CapabilitySource::Framework { capability, .. } => capability.to_string(),
                CapabilitySource::Builtin { capability, .. } => capability.to_string(),
                CapabilitySource::Namespace { capability, .. } => capability.to_string(),
                CapabilitySource::OfferAggregate { capability, .. } => capability.to_string(),
                CapabilitySource::Capability { source_capability, .. } =>
                    format!("{}", source_capability),
                CapabilitySource::CollectionAggregate {
                    capability,
                    collections,
                    component,
                    ..
                } => {
                    format!(
                        "{} from collections '{}' of component '{}'",
                        capability,
                        collections.join(","),
                        &component.abs_moniker
                    )
                }
                Self::FilteredService { capability, component, .. } => {
                    format!("{} '{}'", capability, component.abs_moniker)
                }
            }
        )
    }
}

/// A provider of a capability from an aggregation of one or more collections.
///
/// This trait type-erases the capability type, so it can be handled and hosted generically.
#[async_trait]
pub trait CollectionAggregateCapabilityProvider<C: ComponentInstanceInterface>:
    Send + Sync
{
    /// Lists the instances of the capability.
    ///
    /// The instance is an opaque identifier that is only meaningful for a subsequent
    /// call to `route_instance`.
    async fn list_instances(&self) -> Result<Vec<ChildMoniker>, RoutingError>;

    /// Route the given `instance` of the capability to its source.
    async fn route_instance(
        &self,
        instance: &ChildMoniker,
    ) -> Result<CapabilitySource<C>, RoutingError>;

    /// Trait-object compatible clone.
    fn clone_boxed(&self) -> Box<dyn CollectionAggregateCapabilityProvider<C>>;
}

impl<C: ComponentInstanceInterface> Clone for Box<dyn CollectionAggregateCapabilityProvider<C>> {
    fn clone(&self) -> Self {
        self.clone_boxed()
    }
}

impl<C> fmt::Debug for Box<dyn CollectionAggregateCapabilityProvider<C>> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Box<dyn CollectionAggregateCapabilityProvider>").finish()
    }
}

/// A provider of a capability from an aggregation of zero or more offered instances of a capability.
///
/// This trait type-erases the capability type, so it can be handled and hosted generically.
#[async_trait]
pub trait OfferAggregateCapabilityProvider<C: ComponentInstanceInterface>: Send + Sync {
    /// Lists the instances of the capability.
    ///
    /// The instance is an opaque identifier that is only meaningful for a subsequent
    /// call to `route_instance`.
    async fn list_instances(&self) -> Result<Vec<String>, RoutingError>;

    /// Route the given `instance` of the capability to its source.
    async fn route_instance(&self, instance: &str) -> Result<CapabilitySource<C>, RoutingError>;

    /// Trait-object compatible clone.
    fn clone_boxed(&self) -> Box<dyn OfferAggregateCapabilityProvider<C>>;
}

impl<C: ComponentInstanceInterface> Clone for Box<dyn OfferAggregateCapabilityProvider<C>> {
    fn clone(&self) -> Self {
        self.clone_boxed()
    }
}

impl<C> fmt::Debug for Box<dyn OfferAggregateCapabilityProvider<C>> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Box<dyn OfferAggregateCapabilityProvider>").finish()
    }
}

/// Describes a capability provided by the component manager which could be a framework capability
/// scoped to a realm, a built-in global capability, or a capability from component manager's own
/// namespace.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum InternalCapability {
    Service(Name),
    Protocol(Name),
    Directory(Name),
    Runner(Name),
    EventStream(Name),
    Resolver(Name),
    Storage(Name),
}

impl InternalCapability {
    /// Returns whether the given InternalCapability can be available in a component's namespace.
    pub fn can_be_in_namespace(&self) -> bool {
        matches!(
            self,
            InternalCapability::Service(_)
                | InternalCapability::Protocol(_)
                | InternalCapability::Directory(_)
        )
    }

    /// Returns a name for the capability type.
    pub fn type_name(&self) -> CapabilityTypeName {
        match self {
            InternalCapability::Service(_) => CapabilityTypeName::Service,
            InternalCapability::Protocol(_) => CapabilityTypeName::Protocol,
            InternalCapability::Directory(_) => CapabilityTypeName::Directory,
            InternalCapability::Runner(_) => CapabilityTypeName::Runner,
            InternalCapability::EventStream(_) => CapabilityTypeName::EventStream,
            InternalCapability::Resolver(_) => CapabilityTypeName::Resolver,
            InternalCapability::Storage(_) => CapabilityTypeName::Storage,
        }
    }

    pub fn source_name(&self) -> &Name {
        match self {
            InternalCapability::Service(name) => &name,
            InternalCapability::Protocol(name) => &name,
            InternalCapability::Directory(name) => &name,
            InternalCapability::Runner(name) => &name,
            InternalCapability::EventStream(name) => &name,
            InternalCapability::Resolver(name) => &name,
            InternalCapability::Storage(name) => &name,
        }
    }

    /// Returns true if this is a protocol with name that matches `name`.
    pub fn matches_protocol(&self, name: &Name) -> bool {
        match self {
            Self::Protocol(source_name) => source_name == name,
            _ => false,
        }
    }
}

impl fmt::Display for InternalCapability {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} '{}' from component manager", self.type_name(), self.source_name())
    }
}

impl From<ServiceDecl> for InternalCapability {
    fn from(service: ServiceDecl) -> Self {
        Self::Service(service.name)
    }
}

impl From<ProtocolDecl> for InternalCapability {
    fn from(protocol: ProtocolDecl) -> Self {
        Self::Protocol(protocol.name)
    }
}

impl From<DirectoryDecl> for InternalCapability {
    fn from(directory: DirectoryDecl) -> Self {
        Self::Directory(directory.name)
    }
}

impl From<RunnerDecl> for InternalCapability {
    fn from(runner: RunnerDecl) -> Self {
        Self::Runner(runner.name)
    }
}

impl From<ResolverDecl> for InternalCapability {
    fn from(resolver: ResolverDecl) -> Self {
        Self::Resolver(resolver.name)
    }
}

impl From<EventStreamDecl> for InternalCapability {
    fn from(event: EventStreamDecl) -> Self {
        Self::EventStream(event.name)
    }
}

impl From<StorageDecl> for InternalCapability {
    fn from(storage: StorageDecl) -> Self {
        Self::Storage(storage.name)
    }
}

/// A capability being routed from a component.
#[derive(FromEnum, Clone, Debug, PartialEq, Eq)]
pub enum ComponentCapability {
    Use(UseDecl),
    /// Models a capability used from the environment.
    Environment(EnvironmentCapability),
    Expose(ExposeDecl),
    Offer(OfferDecl),
    Protocol(ProtocolDecl),
    Directory(DirectoryDecl),
    Storage(StorageDecl),
    Runner(RunnerDecl),
    Resolver(ResolverDecl),
    Service(ServiceDecl),
    EventStream(EventStreamDecl),
}

impl ComponentCapability {
    /// Returns whether the given ComponentCapability can be available in a component's namespace.
    pub fn can_be_in_namespace(&self) -> bool {
        match self {
            ComponentCapability::Use(use_) => {
                matches!(use_, UseDecl::Protocol(_) | UseDecl::Directory(_) | UseDecl::Service(_))
            }
            ComponentCapability::Expose(expose) => matches!(
                expose,
                ExposeDecl::Protocol(_) | ExposeDecl::Directory(_) | ExposeDecl::Service(_)
            ),
            ComponentCapability::Offer(offer) => matches!(
                offer,
                OfferDecl::Protocol(_) | OfferDecl::Directory(_) | OfferDecl::Service(_)
            ),
            ComponentCapability::Protocol(_)
            | ComponentCapability::Directory(_)
            | ComponentCapability::Service(_) => true,
            _ => false,
        }
    }

    /// Returns a name for the capability type.
    pub fn type_name(&self) -> CapabilityTypeName {
        match self {
            ComponentCapability::Use(use_) => match use_ {
                UseDecl::Protocol(_) => CapabilityTypeName::Protocol,
                UseDecl::Directory(_) => CapabilityTypeName::Directory,
                UseDecl::Service(_) => CapabilityTypeName::Service,
                UseDecl::Storage(_) => CapabilityTypeName::Storage,
                UseDecl::EventStream(_) => CapabilityTypeName::EventStream,
            },
            ComponentCapability::Environment(env) => match env {
                EnvironmentCapability::Runner { .. } => CapabilityTypeName::Runner,
                EnvironmentCapability::Resolver { .. } => CapabilityTypeName::Resolver,
                EnvironmentCapability::Debug { .. } => CapabilityTypeName::Protocol,
            },
            ComponentCapability::Expose(expose) => match expose {
                ExposeDecl::Protocol(_) => CapabilityTypeName::Protocol,
                ExposeDecl::Directory(_) => CapabilityTypeName::Directory,
                ExposeDecl::Service(_) => CapabilityTypeName::Service,
                ExposeDecl::Runner(_) => CapabilityTypeName::Runner,
                ExposeDecl::Resolver(_) => CapabilityTypeName::Resolver,
            },
            ComponentCapability::Offer(offer) => match offer {
                OfferDecl::Protocol(_) => CapabilityTypeName::Protocol,
                OfferDecl::Directory(_) => CapabilityTypeName::Directory,
                OfferDecl::Service(_) => CapabilityTypeName::Service,
                OfferDecl::Storage(_) => CapabilityTypeName::Storage,
                OfferDecl::Runner(_) => CapabilityTypeName::Runner,
                OfferDecl::Resolver(_) => CapabilityTypeName::Resolver,
                OfferDecl::EventStream(_) => CapabilityTypeName::EventStream,
            },
            ComponentCapability::Protocol(_) => CapabilityTypeName::Protocol,
            ComponentCapability::Directory(_) => CapabilityTypeName::Directory,
            ComponentCapability::Storage(_) => CapabilityTypeName::Storage,
            ComponentCapability::Runner(_) => CapabilityTypeName::Runner,
            ComponentCapability::Resolver(_) => CapabilityTypeName::Resolver,
            ComponentCapability::Service(_) => CapabilityTypeName::Service,
            ComponentCapability::EventStream(_) => CapabilityTypeName::EventStream,
        }
    }

    /// Return the source path of the capability, if one exists.
    pub fn source_path(&self) -> Option<&Path> {
        match self {
            ComponentCapability::Storage(_) => None,
            ComponentCapability::Protocol(protocol) => protocol.source_path.as_ref(),
            ComponentCapability::Directory(directory) => directory.source_path.as_ref(),
            ComponentCapability::Runner(runner) => runner.source_path.as_ref(),
            ComponentCapability::Resolver(resolver) => resolver.source_path.as_ref(),
            ComponentCapability::Service(service) => service.source_path.as_ref(),
            _ => None,
        }
    }

    /// Return the name of the capability, if this is a capability declaration.
    pub fn source_name(&self) -> Option<&Name> {
        match self {
            ComponentCapability::Storage(storage) => Some(&storage.name),
            ComponentCapability::Protocol(protocol) => Some(&protocol.name),
            ComponentCapability::Directory(directory) => Some(&directory.name),
            ComponentCapability::Runner(runner) => Some(&runner.name),
            ComponentCapability::Resolver(resolver) => Some(&resolver.name),
            ComponentCapability::Service(service) => Some(&service.name),
            ComponentCapability::EventStream(event) => Some(&event.name),
            ComponentCapability::Use(use_) => match use_ {
                UseDecl::Protocol(UseProtocolDecl { source_name, .. }) => Some(source_name),
                UseDecl::Directory(UseDirectoryDecl { source_name, .. }) => Some(source_name),
                UseDecl::Storage(UseStorageDecl { source_name, .. }) => Some(source_name),
                UseDecl::Service(UseServiceDecl { source_name, .. }) => Some(source_name),
                _ => None,
            },
            ComponentCapability::Environment(env_cap) => match env_cap {
                EnvironmentCapability::Runner { source_name, .. } => Some(source_name),
                EnvironmentCapability::Resolver { source_name, .. } => Some(source_name),
                EnvironmentCapability::Debug { source_name, .. } => Some(source_name),
            },
            ComponentCapability::Expose(expose) => match expose {
                ExposeDecl::Protocol(ExposeProtocolDecl { source_name, .. }) => Some(source_name),
                ExposeDecl::Directory(ExposeDirectoryDecl { source_name, .. }) => Some(source_name),
                ExposeDecl::Runner(ExposeRunnerDecl { source_name, .. }) => Some(source_name),
                ExposeDecl::Resolver(ExposeResolverDecl { source_name, .. }) => Some(source_name),
                ExposeDecl::Service(ExposeServiceDecl { source_name, .. }) => Some(source_name),
            },
            ComponentCapability::Offer(offer) => match offer {
                OfferDecl::Protocol(OfferProtocolDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Directory(OfferDirectoryDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Runner(OfferRunnerDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Storage(OfferStorageDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Resolver(OfferResolverDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Service(OfferServiceDecl { source_name, .. }) => Some(source_name),
                OfferDecl::EventStream(OfferEventStreamDecl { source_name, .. }) => {
                    Some(source_name)
                }
            },
        }
    }

    pub fn source_capability_name(&self) -> Option<&Name> {
        match self {
            ComponentCapability::Offer(OfferDecl::Protocol(OfferProtocolDecl {
                source: OfferSource::Capability(name),
                ..
            })) => Some(name),
            ComponentCapability::Expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                source: ExposeSource::Capability(name),
                ..
            })) => Some(name),
            ComponentCapability::Use(UseDecl::Protocol(UseProtocolDecl {
                source: UseSource::Capability(name),
                ..
            })) => Some(name),
            _ => None,
        }
    }

    /// Returns the path or name of the capability as a string, useful for debugging.
    pub fn source_id(&self) -> String {
        self.source_name()
            .map(|p| format!("{}", p))
            .or_else(|| self.source_path().map(|p| format!("{}", p)))
            .unwrap_or_default()
    }
}

impl fmt::Display for ComponentCapability {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} '{}' from component", self.type_name(), self.source_id())
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum EnvironmentCapability {
    Runner { source_name: Name, source: RegistrationSource },
    Resolver { source_name: Name, source: RegistrationSource },
    Debug { source_name: Name, source: RegistrationSource },
}

impl EnvironmentCapability {
    pub fn registration_source(&self) -> &RegistrationSource {
        match self {
            Self::Runner { source, .. }
            | Self::Resolver { source, .. }
            | Self::Debug { source, .. } => &source,
        }
    }
}

/// Describes a capability provided by component manager that is an aggregation
/// of multiple instances of a capability.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum AggregateCapability {
    Service(Name),
}

impl AggregateCapability {
    /// Returns true if the AggregateCapability can be available in a component's namespace.
    pub fn can_be_in_namespace(&self) -> bool {
        matches!(self, AggregateCapability::Service(_))
    }

    /// Returns a name for the capability type.
    pub fn type_name(&self) -> CapabilityTypeName {
        match self {
            AggregateCapability::Service(_) => CapabilityTypeName::Service,
        }
    }

    pub fn source_name(&self) -> &Name {
        match self {
            AggregateCapability::Service(name) => &name,
        }
    }
}

impl fmt::Display for AggregateCapability {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "aggregate {} '{}'", self.type_name(), self.source_name())
    }
}

impl From<ServiceDecl> for AggregateCapability {
    fn from(service: ServiceDecl) -> Self {
        Self::Service(service.name)
    }
}

/// The list of declarations for capabilities from component manager's namespace.
pub type NamespaceCapabilities = Vec<CapabilityDecl>;

/// The list of declarations for capabilities offered by component manager as built-in capabilities.
pub type BuiltinCapabilities = Vec<CapabilityDecl>;

#[cfg(test)]
mod tests {
    use {super::*, cm_rust::StorageDirectorySource, fidl_fuchsia_component_decl as fdecl};

    #[test]
    fn capability_type_name() {
        let storage_capability = ComponentCapability::Storage(StorageDecl {
            name: "foo".parse().unwrap(),
            source: StorageDirectorySource::Parent,
            backing_dir: "bar".parse().unwrap(),
            subdir: None,
            storage_id: fdecl::StorageId::StaticInstanceIdOrMoniker,
        });
        assert_eq!(storage_capability.type_name(), CapabilityTypeName::Storage);
    }
}
