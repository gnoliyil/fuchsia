// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::{
        actions::{Action, ActionKey},
        component::{ComponentInstance, InstanceState},
        error::DiscoverError,
        hooks::{Event, EventPayload},
    },
    async_trait::async_trait,
    std::sync::Arc,
};

/// Dispatches a `Discovered` event for a component instance. This action should be registered
/// when a component instance is created.
pub struct DiscoverAction {}

impl DiscoverAction {
    pub fn new() -> Self {
        Self {}
    }
}

#[async_trait]
impl Action for DiscoverAction {
    type Output = Result<(), DiscoverError>;
    async fn handle(&self, component: &Arc<ComponentInstance>) -> Self::Output {
        do_discover(component).await
    }
    fn key(&self) -> ActionKey {
        ActionKey::Discover
    }
}

async fn do_discover(component: &Arc<ComponentInstance>) -> Result<(), DiscoverError> {
    let is_discovered = {
        let state = component.lock_state().await;
        match *state {
            InstanceState::New => false,
            InstanceState::Unresolved => true,
            InstanceState::Resolved(_) => true,
            InstanceState::Destroyed => {
                return Err(DiscoverError::InstanceDestroyed {
                    moniker: component.abs_moniker.clone(),
                });
            }
        }
    };
    if is_discovered {
        return Ok(());
    }
    let event = Event::new(&component, EventPayload::Discovered);
    component.hooks.dispatch(&event).await;
    {
        let mut state = component.lock_state().await;
        assert!(
            matches!(*state, InstanceState::New | InstanceState::Destroyed),
            "Component in unexpected state after discover"
        );
        match *state {
            InstanceState::Destroyed => {
                // Nothing to do.
            }
            InstanceState::Unresolved | InstanceState::Resolved(_) => {
                panic!(
                    "Component was marked {:?} during Discover action, which shouldn't be possible",
                    *state
                );
            }
            InstanceState::New => {
                state.set(InstanceState::Unresolved);
            }
        }
    }
    Ok(())
}
