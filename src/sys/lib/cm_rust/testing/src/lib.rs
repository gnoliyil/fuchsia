// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context, Error},
    cm_rust::{ComponentDecl, FidlIntoNative},
    cml, fidl_fuchsia_component_decl as fdecl, fidl_fuchsia_data as fdata, fidl_fuchsia_io as fio,
};

/// Name of the test runner.
///
/// Several functions assume the existence of a runner with this name.
pub const TEST_RUNNER_NAME: &str = "test_runner";

/// Deserialize `object` into a cml::Document and then translate the result
/// to ComponentDecl.
pub fn new_decl_from_json(object: serde_json::Value) -> Result<ComponentDecl, Error> {
    let doc = serde_json::from_value(object).context("failed to deserialize manifest")?;
    let cm =
        cml::compile(&doc, cml::CompileOptions::default()).context("failed to compile manifest")?;
    Ok(cm.fidl_into_native())
}

/// Builder for constructing a ComponentDecl.
#[derive(Debug, Clone)]
pub struct ComponentDeclBuilder {
    result: ComponentDecl,
}

impl ComponentDeclBuilder {
    /// An empty ComponentDeclBuilder, with no program.
    pub fn new_empty_component() -> Self {
        ComponentDeclBuilder { result: Default::default() }
    }

    /// A ComponentDeclBuilder prefilled with a program and using a runner named "test_runner",
    /// which we assume is offered to us.
    pub fn new() -> Self {
        Self::new_empty_component().add_program(TEST_RUNNER_NAME)
    }

    /// Add a child element.
    pub fn add_child(mut self, decl: impl Into<cm_rust::ChildDecl>) -> Self {
        self.result.children.push(decl.into());
        self
    }

    // Add a collection element.
    pub fn add_collection(mut self, decl: impl Into<cm_rust::CollectionDecl>) -> Self {
        self.result.collections.push(decl.into());
        self
    }

    /// Add a lazily instantiated child with a default test URL derived from the name.
    pub fn add_lazy_child(self, name: &str) -> Self {
        self.add_child(ChildDeclBuilder::new_lazy_child(name))
    }

    /// Add an eagerly instantiated child with a default test URL derived from the name.
    pub fn add_eager_child(self, name: &str) -> Self {
        self.add_child(
            ChildDeclBuilder::new()
                .name(name)
                .url(&format!("test:///{}", name))
                .startup(fdecl::StartupMode::Eager),
        )
    }

    /// Add a transient collection.
    pub fn add_transient_collection(self, name: &str) -> Self {
        self.add_collection(CollectionDeclBuilder::new_transient_collection(name))
    }

    /// Add a single run collection.
    pub fn add_single_run_collection(self, name: &str) -> Self {
        self.add_collection(CollectionDeclBuilder::new_single_run_collection(name))
    }

    /// Add a "program" clause, using the given runner.
    pub fn add_program(mut self, runner: &str) -> Self {
        assert!(self.result.program.is_none(), "tried to add program twice");
        self.result.program = Some(cm_rust::ProgramDecl {
            runner: Some(runner.parse().unwrap()),
            info: fdata::Dictionary { entries: Some(vec![]), ..Default::default() },
        });
        self
    }

    /// Add a custom offer.
    pub fn offer(mut self, offer: cm_rust::OfferDecl) -> Self {
        self.result.offers.push(offer);
        self
    }

    /// Add a custom expose.
    pub fn expose(mut self, expose: cm_rust::ExposeDecl) -> Self {
        self.result.exposes.push(expose);
        self
    }

    /// Add a custom use decl.
    pub fn use_(mut self, use_: cm_rust::UseDecl) -> Self {
        self.result.uses.push(use_);
        self
    }

    // Add a use decl for fuchsia.component.Realm.
    pub fn use_realm(mut self) -> Self {
        let use_ = cm_rust::UseDecl::Protocol(cm_rust::UseProtocolDecl {
            dependency_type: cm_rust::DependencyType::Strong,
            source: cm_rust::UseSource::Framework,
            source_name: "fuchsia.component.Realm".parse().unwrap(),
            target_path: "/svc/fuchsia.component.Realm".parse().unwrap(),
            availability: cm_rust::Availability::Required,
        });
        self.result.uses.push(use_);
        self
    }

    /// Add a custom protocol declaration.
    pub fn protocol(mut self, protocol: cm_rust::ProtocolDecl) -> Self {
        self.result.capabilities.push(cm_rust::CapabilityDecl::Protocol(protocol));
        self
    }

    /// Add a custom directory declaration.
    pub fn directory(mut self, directory: cm_rust::DirectoryDecl) -> Self {
        self.result.capabilities.push(cm_rust::CapabilityDecl::Directory(directory));
        self
    }

    /// Add a custom storage declaration.
    pub fn storage(mut self, storage: cm_rust::StorageDecl) -> Self {
        self.result.capabilities.push(cm_rust::CapabilityDecl::Storage(storage));
        self
    }

    /// Add a custom runner declaration.
    pub fn runner(mut self, runner: cm_rust::RunnerDecl) -> Self {
        self.result.capabilities.push(cm_rust::CapabilityDecl::Runner(runner));
        self
    }

    /// Add a custom resolver declaration.
    pub fn resolver(mut self, resolver: cm_rust::ResolverDecl) -> Self {
        self.result.capabilities.push(cm_rust::CapabilityDecl::Resolver(resolver));
        self
    }

    /// Add a custom service declaration.
    pub fn service(mut self, service: cm_rust::ServiceDecl) -> Self {
        self.result.capabilities.push(cm_rust::CapabilityDecl::Service(service));
        self
    }

    /// Add an environment declaration.
    pub fn add_environment(mut self, environment: impl Into<cm_rust::EnvironmentDecl>) -> Self {
        self.result.environments.push(environment.into());
        self
    }

    /// Add a config declaration.
    pub fn add_config(mut self, config: cm_rust::ConfigDecl) -> Self {
        self.result.config = Some(config);
        self
    }

    /// Generate the final ComponentDecl.
    pub fn build(self) -> ComponentDecl {
        self.result
    }
}

/// A convenience builder for constructing ChildDecls.
#[derive(Debug)]
pub struct ChildDeclBuilder(cm_rust::ChildDecl);

impl ChildDeclBuilder {
    /// Creates a new builder.
    pub fn new() -> Self {
        ChildDeclBuilder(cm_rust::ChildDecl {
            name: String::new(),
            url: String::new(),
            startup: fdecl::StartupMode::Lazy,
            environment: None,
            on_terminate: None,
            config_overrides: None,
        })
    }

    /// Creates a new builder initialized with a lazy child.
    pub fn new_lazy_child(name: &str) -> Self {
        Self::new().name(name).url(&format!("test:///{}", name)).startup(fdecl::StartupMode::Lazy)
    }

    /// Sets the ChildDecl's name.
    pub fn name(mut self, name: &str) -> Self {
        self.0.name = name.to_string();
        self
    }

    /// Sets the ChildDecl's url.
    pub fn url(mut self, url: &str) -> Self {
        self.0.url = url.to_string();
        self
    }

    /// Sets the ChildDecl's startup mode.
    pub fn startup(mut self, startup: fdecl::StartupMode) -> Self {
        self.0.startup = startup;
        self
    }

    /// Sets the ChildDecl's on_terminate action.
    pub fn on_terminate(mut self, on_terminate: fdecl::OnTerminate) -> Self {
        self.0.on_terminate = Some(on_terminate);
        self
    }

    /// Sets the ChildDecl's environment name.
    pub fn environment(mut self, environment: &str) -> Self {
        self.0.environment = Some(environment.to_string());
        self
    }

    /// Consumes the builder and returns a ChildDecl.
    pub fn build(mut self) -> cm_rust::ChildDecl {
        if self.0.url == String::new() {
            self.0.url = format!("test://{}", self.0.name);
        }
        self.0
    }
}

impl From<ChildDeclBuilder> for cm_rust::ChildDecl {
    fn from(builder: ChildDeclBuilder) -> Self {
        builder.build()
    }
}

/// A convenience builder for constructing CollectionDecls.
#[derive(Debug)]
pub struct CollectionDeclBuilder(cm_rust::CollectionDecl);

impl CollectionDeclBuilder {
    /// Creates a new builder.
    pub fn new() -> Self {
        CollectionDeclBuilder(cm_rust::CollectionDecl {
            name: String::new(),
            durability: fdecl::Durability::Transient,
            environment: None,
            allowed_offers: cm_types::AllowedOffers::StaticOnly,
            allow_long_names: false,
            persistent_storage: None,
        })
    }

    /// Creates a new builder initialized with a transient collection.
    pub fn new_transient_collection(name: &str) -> Self {
        Self::new().name(name).durability(fdecl::Durability::Transient)
    }

    /// Creates a new builder initialized with a single run collection.
    pub fn new_single_run_collection(name: &str) -> Self {
        Self::new().name(name).durability(fdecl::Durability::SingleRun)
    }

    /// Sets the CollectionDecl's name.
    pub fn name(mut self, name: &str) -> Self {
        self.0.name = name.to_string();
        self
    }

    /// Sets the CollectionDecl's durability
    pub fn durability(mut self, durability: fdecl::Durability) -> Self {
        self.0.durability = durability;
        self
    }

    /// Sets the CollectionDecl's environment name.
    pub fn environment(mut self, environment: &str) -> Self {
        self.0.environment = Some(environment.to_string());
        self
    }

    /// Sets the kinds of offers that may target the instances in the
    /// collection.
    pub fn allowed_offers(mut self, allowed_offers: cm_types::AllowedOffers) -> Self {
        self.0.allowed_offers = allowed_offers;
        self
    }

    // Sets the flag to allow the collection to have child names that exceed the default length
    // limit.
    pub fn allow_long_names(mut self, allow_long_names: bool) -> Self {
        self.0.allow_long_names = allow_long_names;
        self
    }

    // Sets the flag to persist isolated storage data of the collection and its descendents.
    pub fn persistent_storage(mut self, persistent_storage: bool) -> Self {
        self.0.persistent_storage = Some(persistent_storage);
        self
    }

    /// Consumes the builder and returns a CollectionDecl.
    pub fn build(self) -> cm_rust::CollectionDecl {
        self.0
    }
}

impl From<CollectionDeclBuilder> for cm_rust::CollectionDecl {
    fn from(builder: CollectionDeclBuilder) -> Self {
        builder.build()
    }
}

/// A convenience builder for constructing EnvironmentDecls.
#[derive(Debug)]
pub struct EnvironmentDeclBuilder(cm_rust::EnvironmentDecl);

impl EnvironmentDeclBuilder {
    /// Creates a new builder.
    pub fn new() -> Self {
        EnvironmentDeclBuilder(cm_rust::EnvironmentDecl {
            name: String::new(),
            extends: fdecl::EnvironmentExtends::None,
            runners: vec![],
            resolvers: vec![],
            debug_capabilities: vec![],
            stop_timeout_ms: None,
        })
    }

    /// Sets the EnvironmentDecl's name.
    pub fn name(mut self, name: &str) -> Self {
        self.0.name = name.to_string();
        self
    }

    /// Sets whether the environment extends from its realm.
    pub fn extends(mut self, extends: fdecl::EnvironmentExtends) -> Self {
        self.0.extends = extends;
        self
    }

    /// Registers a runner with the environment.
    pub fn add_runner(mut self, runner: cm_rust::RunnerRegistration) -> Self {
        self.0.runners.push(runner);
        self
    }

    /// Registers a resolver with the environment.
    pub fn add_resolver(mut self, resolver: cm_rust::ResolverRegistration) -> Self {
        self.0.resolvers.push(resolver);
        self
    }

    /// Registers a debug capability with the environment.
    pub fn add_debug_registration(mut self, debug: cm_rust::DebugRegistration) -> Self {
        self.0.debug_capabilities.push(debug);
        self
    }

    pub fn stop_timeout(mut self, timeout_ms: u32) -> Self {
        self.0.stop_timeout_ms = Some(timeout_ms);
        self
    }

    /// Consumes the builder and returns an EnvironmentDecl.
    pub fn build(self) -> cm_rust::EnvironmentDecl {
        self.0
    }
}

impl From<EnvironmentDeclBuilder> for cm_rust::EnvironmentDecl {
    fn from(builder: EnvironmentDeclBuilder) -> Self {
        builder.build()
    }
}

// A convenience builder for constructing ProtocolDecls.
#[derive(Debug)]
pub struct ProtocolDeclBuilder(cm_rust::ProtocolDecl);

impl ProtocolDeclBuilder {
    /// Creates a new builder.
    pub fn new(name: &str) -> Self {
        Self(cm_rust::ProtocolDecl {
            name: name.parse().unwrap(),
            source_path: Some(format!("/svc/foo").parse().unwrap()),
        })
    }

    /// Sets the source path.
    pub fn path(mut self, path: &str) -> Self {
        self.0.source_path = Some(path.parse().unwrap());
        self
    }

    /// Consumes the builder and returns a ProtocolDecl.
    pub fn build(self) -> cm_rust::ProtocolDecl {
        self.0
    }
}

impl From<ProtocolDeclBuilder> for cm_rust::ProtocolDecl {
    fn from(builder: ProtocolDeclBuilder) -> Self {
        builder.build()
    }
}

// A convenience builder for constructing ServiceDecls.
#[derive(Debug)]
pub struct ServiceDeclBuilder(cm_rust::ServiceDecl);

impl ServiceDeclBuilder {
    /// Creates a new builder.
    pub fn new(name: &str) -> Self {
        Self(cm_rust::ServiceDecl {
            name: name.parse().unwrap(),
            source_path: Some(format!("/svc/foo.service").parse().unwrap()),
        })
    }

    /// Sets the source path.
    pub fn path(mut self, path: &str) -> Self {
        self.0.source_path = Some(path.parse().unwrap());
        self
    }

    /// Consumes the builder and returns a ServiceDecl.
    pub fn build(self) -> cm_rust::ServiceDecl {
        self.0
    }
}

impl From<ServiceDeclBuilder> for cm_rust::ServiceDecl {
    fn from(builder: ServiceDeclBuilder) -> Self {
        builder.build()
    }
}

// A convenience builder for constructing DirectoryDecls.
#[derive(Debug)]
pub struct DirectoryDeclBuilder(cm_rust::DirectoryDecl);

impl DirectoryDeclBuilder {
    /// Creates a new builder.
    pub fn new(name: &str) -> Self {
        Self(cm_rust::DirectoryDecl {
            name: name.parse().unwrap(),
            source_path: Some(format!("/data/foo").parse().unwrap()),
            rights: fio::R_STAR_DIR,
        })
    }

    /// Sets the source path.
    pub fn path(mut self, path: &str) -> Self {
        self.0.source_path = Some(path.parse().unwrap());
        self
    }

    /// Sets the rights.
    pub fn rights(mut self, rights: fio::Operations) -> Self {
        self.0.rights = rights;
        self
    }

    /// Consumes the builder and returns a DirectoryDecl.
    pub fn build(self) -> cm_rust::DirectoryDecl {
        self.0
    }
}

impl From<DirectoryDeclBuilder> for cm_rust::DirectoryDecl {
    fn from(builder: DirectoryDeclBuilder) -> Self {
        builder.build()
    }
}
