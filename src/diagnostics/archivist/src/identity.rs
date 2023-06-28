// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::events::types::{ComponentIdentifier, Moniker};
use diagnostics_message::MonikerWithUrl;
use flyweights::FlyStr;

#[derive(Debug, Eq, Hash, PartialEq)]
pub struct ComponentIdentity {
    /// Relative moniker of the component that this artifacts container
    /// is representing.
    pub relative_moniker: Moniker,

    /// Instance id (only set for v1 components)
    pub instance_id: Option<Box<str>>,

    /// The url with which the associated component was launched.
    pub url: FlyStr,
}

impl ComponentIdentity {
    pub fn from_identifier_and_url(
        identifier: ComponentIdentifier,
        url: impl Into<FlyStr>,
    ) -> Self {
        ComponentIdentity {
            relative_moniker: identifier.relative_moniker_for_selectors(),
            instance_id: match identifier {
                ComponentIdentifier::Moniker(..) => None,
            },
            url: url.into(),
        }
    }

    /// Returns generic metadata, suitable for providing a uniform ID to unattributed data.
    pub fn unknown() -> Self {
        Self::from_identifier_and_url(
            ComponentIdentifier::parse_from_moniker("./UNKNOWN").unwrap(),
            "fuchsia-pkg://UNKNOWN",
        )
    }
}

#[cfg(test)]
impl From<Vec<&str>> for ComponentIdentity {
    fn from(moniker_segments: Vec<&str>) -> Self {
        Self { relative_moniker: moniker_segments.into(), instance_id: None, url: "".into() }
    }
}

impl From<ComponentIdentity> for MonikerWithUrl {
    fn from(identity: ComponentIdentity) -> Self {
        Self { moniker: identity.to_string(), url: identity.url.into() }
    }
}

impl From<&ComponentIdentity> for MonikerWithUrl {
    fn from(identity: &ComponentIdentity) -> Self {
        Self { moniker: identity.to_string(), url: identity.url.to_string() }
    }
}

impl std::fmt::Display for ComponentIdentity {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.relative_moniker.fmt(f)
    }
}
