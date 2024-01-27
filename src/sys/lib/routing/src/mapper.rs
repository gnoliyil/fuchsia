// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::RegistrationDecl,
    cm_rust::{CapabilityDecl, CapabilityName, ExposeDecl, OfferDecl, UseDecl},
    moniker::AbsoluteMoniker,
};

#[cfg(feature = "serde")]
use serde::{Deserialize, Serialize};

/// Describes a single step taken by the capability routing algorithm.
#[cfg_attr(
    feature = "serde",
    derive(Deserialize, Serialize),
    serde(rename_all = "snake_case", tag = "action")
)]
#[derive(Clone, Debug, PartialEq)]
pub enum RouteSegment {
    /// The capability was used by a component instance in its manifest.
    UseBy { moniker: AbsoluteMoniker, capability: UseDecl },

    /// The capability was offered by a component instance in its manifest.
    OfferBy { moniker: AbsoluteMoniker, capability: OfferDecl },

    /// The capability was exposed by a component instance in its manifest.
    ExposeBy { moniker: AbsoluteMoniker, capability: ExposeDecl },

    /// The capability was declared by a component instance in its manifest.
    DeclareBy { moniker: AbsoluteMoniker, capability: CapabilityDecl },

    /// The capability was registered in a component instance's environment in its manifest.
    RegisterBy { moniker: AbsoluteMoniker, capability: RegistrationDecl },

    /// This is a framework capability served by component manager.
    ProvideFromFramework { capability: CapabilityName },

    /// This is a builtin capability served by component manager.
    ProvideAsBuiltin { capability: CapabilityDecl },

    /// This is a capability available in component manager's namespace.
    ProvideFromNamespace { capability: CapabilityDecl },
}

impl RouteSegment {
    /// Get the moniker of the component instance where this segment occurred, if any.
    pub fn moniker(&self) -> Option<AbsoluteMoniker> {
        match self {
            Self::UseBy { moniker, .. }
            | Self::DeclareBy { moniker, .. }
            | Self::ExposeBy { moniker, .. }
            | Self::OfferBy { moniker, .. }
            | Self::RegisterBy { moniker, .. } => Some(moniker.clone()),
            _ => None,
        }
    }
}

#[derive(Clone, Debug)]
pub struct RouteMapper {
    route: Vec<RouteSegment>,
}

impl RouteMapper {
    pub fn new() -> Self {
        Self { route: vec![] }
    }

    pub fn get_route(self) -> Vec<RouteSegment> {
        self.route
    }
}

impl DebugRouteMapper for RouteMapper {
    fn add_use(&mut self, abs_moniker: AbsoluteMoniker, use_decl: UseDecl) {
        self.route.push(RouteSegment::UseBy { moniker: abs_moniker, capability: use_decl })
    }

    fn add_offer(&mut self, abs_moniker: AbsoluteMoniker, offer_decl: OfferDecl) {
        self.route.push(RouteSegment::OfferBy { moniker: abs_moniker, capability: offer_decl })
    }

    fn add_expose(&mut self, abs_moniker: AbsoluteMoniker, expose_decl: ExposeDecl) {
        self.route.push(RouteSegment::ExposeBy { moniker: abs_moniker, capability: expose_decl })
    }

    fn add_registration(
        &mut self,
        abs_moniker: AbsoluteMoniker,
        registration_decl: RegistrationDecl,
    ) {
        self.route
            .push(RouteSegment::RegisterBy { moniker: abs_moniker, capability: registration_decl })
    }

    fn add_component_capability(
        &mut self,
        abs_moniker: AbsoluteMoniker,
        capability_decl: CapabilityDecl,
    ) {
        self.route
            .push(RouteSegment::DeclareBy { moniker: abs_moniker, capability: capability_decl })
    }

    fn add_framework_capability(&mut self, capability_name: CapabilityName) {
        self.route.push(RouteSegment::ProvideFromFramework { capability: capability_name })
    }

    fn add_builtin_capability(&mut self, capability_decl: CapabilityDecl) {
        self.route.push(RouteSegment::ProvideAsBuiltin { capability: capability_decl })
    }

    fn add_namespace_capability(&mut self, capability_decl: CapabilityDecl) {
        self.route.push(RouteSegment::ProvideFromNamespace { capability: capability_decl })
    }
}

#[derive(Clone, Debug)]
pub struct NoopRouteMapper;

impl DebugRouteMapper for NoopRouteMapper {}

/// Provides methods to record and retrieve a summary of a capability route.
pub trait DebugRouteMapper: Send + Sync + Clone {
    #[allow(unused_variables)]
    fn add_use(&mut self, abs_moniker: AbsoluteMoniker, use_decl: UseDecl) {}

    #[allow(unused_variables)]
    fn add_offer(&mut self, abs_moniker: AbsoluteMoniker, offer_decl: OfferDecl) {}

    #[allow(unused_variables)]
    fn add_expose(&mut self, abs_moniker: AbsoluteMoniker, expose_decl: ExposeDecl) {}

    #[allow(unused_variables)]
    fn add_registration(
        &mut self,
        abs_moniker: AbsoluteMoniker,
        registration_decl: RegistrationDecl,
    ) {
    }

    #[allow(unused_variables)]
    fn add_component_capability(
        &mut self,
        abs_moniker: AbsoluteMoniker,
        capability_decl: CapabilityDecl,
    ) {
    }

    #[allow(unused_variables)]
    fn add_framework_capability(&mut self, capability_name: CapabilityName) {}

    #[allow(unused_variables)]
    fn add_builtin_capability(&mut self, capability_decl: CapabilityDecl) {}

    #[allow(unused_variables)]
    fn add_namespace_capability(&mut self, capability_decl: CapabilityDecl) {}
}
