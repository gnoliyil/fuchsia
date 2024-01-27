// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::error::ModelError,
    ::routing::{
        component_id_index::ComponentIdIndex,
        config::{AbiRevisionPolicy, RuntimeConfig},
        policy::GlobalPolicyChecker,
    },
    std::sync::{Arc, Weak},
};

/// The ModelContext provides the API boundary between the Model and Realms. It
/// defines what parts of the Model or authoritative state about the tree we
/// want to share with Realms.
pub struct ModelContext {
    policy_checker: GlobalPolicyChecker,
    component_id_index: Arc<ComponentIdIndex>,
    runtime_config: Arc<RuntimeConfig>,
    abi_revision_policy: AbiRevisionPolicy,
}

impl ModelContext {
    /// Constructs a new ModelContext from a RuntimeConfig.
    pub async fn new(runtime_config: Arc<RuntimeConfig>) -> Result<Self, ModelError> {
        Ok(Self {
            component_id_index: match &runtime_config.component_id_index_path {
                Some(path) => Arc::new(ComponentIdIndex::new(&path).await?),
                None => Arc::new(ComponentIdIndex::default()),
            },
            abi_revision_policy: runtime_config.abi_revision_policy.clone(),
            runtime_config: runtime_config.clone(),
            policy_checker: GlobalPolicyChecker::new(runtime_config),
        })
    }

    /// Returns the runtime policy checker for the model.
    pub fn policy(&self) -> &GlobalPolicyChecker {
        &self.policy_checker
    }

    pub fn runtime_config(&self) -> &Arc<RuntimeConfig> {
        &self.runtime_config
    }

    pub fn component_id_index(&self) -> Arc<ComponentIdIndex> {
        self.component_id_index.clone()
    }

    pub fn abi_revision_policy(&self) -> &AbiRevisionPolicy {
        &self.abi_revision_policy
    }
}

/// A wrapper for a weak reference to `ModelContext`. It implements an upgrade()
/// member function that returns a ModelError which is useful for error
/// reporting.
#[derive(Default, Clone)]
pub struct WeakModelContext {
    inner: Weak<ModelContext>,
}

impl From<&Arc<ModelContext>> for WeakModelContext {
    fn from(context: &Arc<ModelContext>) -> Self {
        Self { inner: Arc::downgrade(context) }
    }
}

impl WeakModelContext {
    /// Constructs the weak context wrapper from a weak reference.
    pub fn new(inner: Weak<ModelContext>) -> Self {
        Self { inner }
    }

    /// Attempts to upgrade this `WeakModelContext` into an `Arc<ModelContext>`, if the
    /// context has not been destroyed.
    pub fn upgrade(&self) -> Result<Arc<ModelContext>, ModelError> {
        self.inner.upgrade().ok_or_else(|| ModelError::context_not_found())
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;

    #[fuchsia::test]
    fn weak_context_returns_error() {
        let weak_context = WeakModelContext::new(Weak::new());
        assert!(weak_context.upgrade().is_err());
    }
}
