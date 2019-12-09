// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{capability::*, model::*},
    by_addr::ByAddr,
    cm_rust::{ComponentDecl, UseDecl},
    fuchsia_trace as trace,
    futures::{future::BoxFuture, lock::Mutex},
    std::{
        collections::HashMap,
        fmt,
        sync::{Arc, Weak},
    },
};

#[derive(Debug, Eq, PartialEq, Hash)]
pub enum EventType {
    /// Keep the event types listed below in alphabetical order!

    /// A dynamic child was added to the parent instance.
    /// Depending on its eagerness, this child may/may not be started yet.
    AddDynamicChild,

    /// An instance was destroyed successfully. The instance is stopped and no longer
    /// exists in the parent's realm.
    PostDestroyInstance,

    /// Destruction of an instance has begun. The instance may/may not be stopped by this point.
    /// The instance still exists in the parent's realm but will soon be removed.
    /// TODO(fxb/39417): Ensure the instance is stopped before this event.
    PreDestroyInstance,

    /// The root component instance's declaration was resolved successfully for the first time.
    RootComponentResolved,

    /// A builtin capability is being requested by a component and requires routing.
    /// The event propagation system is used to supply the capability being requested.
    RouteBuiltinCapability,

    /// A framework capability is being requested by a component and requires routing.
    /// The event propagation system is used to supply the capability being requested.
    RouteFrameworkCapability,

    /// An instance was bound to. If the instance is executable, it is also started.
    StartInstance,

    /// An instance was stopped successfully.
    /// This event must occur before PostDestroyInstance.
    StopInstance,

    /// A capability was used by an instance. An instance uses a capability when
    /// it creates a channel and provides the server end to ComponentManager for routing.
    UseCapability,
}

/// The component manager calls out to objects that implement the `Hook` trait on registered
/// component manager events. Hooks block the flow of a task, and can mutate, decorate and replace
/// capabilities. This permits `Hook` to serve as a point of extensibility for the component
/// manager.
pub trait Hook: Send + Sync {
    fn on<'a>(self: Arc<Self>, event: &'a Event) -> BoxFuture<'a, Result<(), ModelError>>;
}

/// An object registers a hook into a component manager event via a `HooksRegistration` object.
/// A single object may register for multiple events through a vector of `EventType`. `Hooks`
/// does not retain the callback. The hook is lazily removed when the callback object loses
/// strong references.
pub struct HooksRegistration {
    pub events: Vec<EventType>,
    pub callback: Weak<dyn Hook>,
}

#[derive(Clone)]
pub enum EventPayload {
    // Keep the events listed below in alphabetical order!
    AddDynamicChild,
    PostDestroyInstance,
    PreDestroyInstance,
    RootComponentResolved,
    RouteBuiltinCapability {
        capability: ComponentManagerCapability,
        // Events are passed to hooks as immutable borrows. In order to mutate,
        // a field within an Event, interior mutability is employed here with
        // a Mutex.
        capability_provider: Arc<Mutex<Option<Box<dyn ComponentManagerCapabilityProvider>>>>,
    },
    RouteFrameworkCapability {
        capability: ComponentManagerCapability,
        // Events are passed to hooks as immutable borrows. In order to mutate,
        // a field within an Event, interior mutability is employed here with
        // a Mutex.
        capability_provider: Arc<Mutex<Option<Box<dyn ComponentManagerCapabilityProvider>>>>,
    },
    StartInstance {
        component_decl: ComponentDecl,
        live_child_realms: Vec<Arc<Realm>>,
        routing_facade: RoutingFacade,
    },
    StopInstance,
    UseCapability {
        use_: UseDecl,
    },
}

impl EventPayload {
    pub fn type_(&self) -> EventType {
        match self {
            EventPayload::AddDynamicChild => EventType::AddDynamicChild,
            EventPayload::PostDestroyInstance => EventType::PostDestroyInstance,
            EventPayload::PreDestroyInstance => EventType::PreDestroyInstance,
            EventPayload::RootComponentResolved => EventType::RootComponentResolved,
            EventPayload::RouteBuiltinCapability { .. } => EventType::RouteBuiltinCapability,
            EventPayload::RouteFrameworkCapability { .. } => EventType::RouteFrameworkCapability,
            EventPayload::StartInstance { .. } => EventType::StartInstance,
            EventPayload::StopInstance => EventType::StopInstance,
            EventPayload::UseCapability { .. } => EventType::UseCapability,
        }
    }
}

impl fmt::Debug for EventPayload {
    fn fmt(&self, fmt: &mut fmt::Formatter) -> fmt::Result {
        let mut formatter = fmt.debug_struct("EventPayload");
        formatter.field("type", &self.type_());
        match self {
            EventPayload::AddDynamicChild
            | EventPayload::PostDestroyInstance
            | EventPayload::PreDestroyInstance
            | EventPayload::RootComponentResolved
            | EventPayload::StopInstance => formatter.finish(),
            EventPayload::RouteBuiltinCapability { capability, .. } => {
                formatter.field("capability", &capability).finish()
            }
            EventPayload::RouteFrameworkCapability { capability, .. } => {
                formatter.field("capability", &capability).finish()
            }
            EventPayload::StartInstance { component_decl, .. } => {
                formatter.field("component_decl", &component_decl).finish()
            }
            EventPayload::UseCapability { use_ } => formatter.field("use_decl", &use_).finish(),
        }
    }
}

#[derive(Clone, Debug)]
pub struct Event {
    pub target_realm: Arc<Realm>,
    pub payload: EventPayload,
}

/// This is a collection of hooks to component manager events.
/// Note: Cloning Hooks results in a shallow copy.
#[derive(Clone)]
pub struct Hooks {
    parent: Option<Box<Hooks>>,
    inner: Arc<Mutex<HooksInner>>,
}

impl Hooks {
    pub fn new(parent: Option<&Hooks>) -> Self {
        Self {
            parent: parent.map(|hooks| Box::new(hooks.clone())),
            inner: Arc::new(Mutex::new(HooksInner::new())),
        }
    }

    pub async fn install(&self, hooks: Vec<HooksRegistration>) {
        let mut inner = self.inner.lock().await;
        for hook in hooks {
            for event in hook.events {
                let event_hooks = &mut inner.hooks.entry(event).or_insert(vec![]);
                // We cannot compare weak pointers, so we won't dedup pointers at
                // install time but at dispatch time when we go to upgrade pointers.
                event_hooks.push(hook.callback.clone());
            }
        }
    }

    pub fn dispatch<'a>(&'a self, event: &'a Event) -> BoxFuture<Result<(), ModelError>> {
        Box::pin(async move {
            // Trace event dispatch
            let event_type = format!("{:?}", event.payload.type_());
            let target_moniker = event.target_realm.abs_moniker.to_string();
            trace::duration!(
                "component_manager",
                "hooks:dispatch",
                "event_type" => event_type.as_str(),
                "target_moniker" => target_moniker.as_str()
            );

            let hooks = {
                // We must upgrade our weak references to hooks to strong ones before we can
                // call out to them. While we're upgrading references, we dedup references
                // here as well. Ideally this would be done at installation time, not
                // dispatch time but comparing weak references is not yet supported.
                let mut strong_hooks: Vec<ByAddr<dyn Hook>> = vec![];
                let mut inner = self.inner.lock().await;
                if let Some(hooks) = inner.hooks.get_mut(&event.payload.type_()) {
                    hooks.retain(|hook| match hook.upgrade() {
                        Some(hook) => {
                            let hook = ByAddr::new(hook);
                            if !strong_hooks.contains(&hook) {
                                strong_hooks.push(hook);
                                true
                            } else {
                                // Don't retain a weak pointer if it is a duplicate.
                                false
                            }
                        }
                        // Don't retain a weak pointer if it cannot be upgraded.
                        None => false,
                    });
                }
                strong_hooks
            };
            for hook in hooks.into_iter() {
                hook.0.on(event).await?;
            }

            if let Some(parent) = &self.parent {
                parent.dispatch(event).await?;
            }
            Ok(())
        })
    }
}

pub struct HooksInner {
    hooks: HashMap<EventType, Vec<Weak<dyn Hook>>>,
}

impl HooksInner {
    pub fn new() -> Self {
        Self { hooks: HashMap::new() }
    }
}

#[cfg(test)]
mod tests {
    use {super::*, std::sync::Arc};

    #[derive(Clone)]
    struct EventLog {
        log: Arc<Mutex<Vec<String>>>,
    }

    impl EventLog {
        pub fn new() -> Self {
            Self { log: Arc::new(Mutex::new(Vec::new())) }
        }

        pub async fn append(&self, item: String) {
            let mut log = self.log.lock().await;
            log.push(item);
        }

        pub async fn get(&self) -> Vec<String> {
            self.log.lock().await.clone()
        }
    }

    struct CallCounter {
        name: String,
        logger: Option<EventLog>,
        call_count: Mutex<i32>,
    }

    impl CallCounter {
        pub fn new(name: &str, logger: Option<EventLog>) -> Arc<Self> {
            Arc::new(Self { name: name.to_string(), logger, call_count: Mutex::new(0) })
        }

        pub async fn count(&self) -> i32 {
            *self.call_count.lock().await
        }

        async fn on_add_dynamic_child_async(&self) -> Result<(), ModelError> {
            let mut call_count = self.call_count.lock().await;
            *call_count += 1;
            if let Some(logger) = &self.logger {
                let fut = logger.append(format!("{}::OnAddDynamicChild", self.name));
                fut.await;
            }
            Ok(())
        }
    }

    impl Hook for CallCounter {
        fn on(self: Arc<Self>, event: &Event) -> BoxFuture<Result<(), ModelError>> {
            Box::pin(async move {
                if let EventPayload::AddDynamicChild = event.payload {
                    self.on_add_dynamic_child_async().await?;
                }
                Ok(())
            })
        }
    }

    fn log(v: Vec<&str>) -> Vec<String> {
        v.iter().map(|s| s.to_string()).collect()
    }

    // This test verifies that a hook cannot be installed twice.
    #[fuchsia_async::run_singlethreaded(test)]
    async fn install_hook_twice() {
        // CallCounter counts the number of AddDynamicChild events it receives.
        // It should only ever receive one.
        let call_counter = CallCounter::new("CallCounter", None);
        let hooks = Hooks::new(None);

        // Attempt to install CallCounter twice.
        hooks
            .install(vec![HooksRegistration {
                events: vec![EventType::AddDynamicChild],
                callback: Arc::downgrade(&call_counter) as Weak<dyn Hook>,
            }])
            .await;
        hooks
            .install(vec![HooksRegistration {
                events: vec![EventType::AddDynamicChild],
                callback: Arc::downgrade(&call_counter) as Weak<dyn Hook>,
            }])
            .await;

        let realm = {
            let resolver = ResolverRegistry::new();
            let root_component_url = "test:///root".to_string();
            Arc::new(Realm::new_root_realm(resolver, root_component_url))
        };
        let event = Event { target_realm: realm.clone(), payload: EventPayload::AddDynamicChild };
        hooks.dispatch(&event).await.expect("Unable to call hooks.");
        assert_eq!(1, call_counter.count().await);
    }

    // This test verifies that events propagate from child_hooks to parent_hooks.
    #[fuchsia_async::run_singlethreaded(test)]
    async fn event_propagation() {
        let parent_hooks = Hooks::new(None);
        let child_hooks = Hooks::new(Some(&parent_hooks));

        let event_log = EventLog::new();
        let parent_call_counter = CallCounter::new("ParentCallCounter", Some(event_log.clone()));
        parent_hooks
            .install(vec![HooksRegistration {
                events: vec![EventType::AddDynamicChild],
                callback: Arc::downgrade(&parent_call_counter) as Weak<dyn Hook>,
            }])
            .await;

        let child_call_counter = CallCounter::new("ChildCallCounter", Some(event_log.clone()));
        child_hooks
            .install(vec![HooksRegistration {
                events: vec![EventType::AddDynamicChild],
                callback: Arc::downgrade(&child_call_counter) as Weak<dyn Hook>,
            }])
            .await;

        assert_eq!(1, Arc::strong_count(&parent_call_counter));
        assert_eq!(1, Arc::strong_count(&child_call_counter));

        let realm = {
            let resolver = ResolverRegistry::new();
            let root_component_url = "test:///root".to_string();
            Arc::new(Realm::new_root_realm(resolver, root_component_url))
        };
        let event = Event { target_realm: realm.clone(), payload: EventPayload::AddDynamicChild };
        child_hooks.dispatch(&event).await.expect("Unable to call hooks.");
        // parent_call_counter gets informed of the event on child_hooks even though it has
        // been installed on parent_hooks.
        assert_eq!(1, parent_call_counter.count().await);
        // child_call_counter should be called only once.
        assert_eq!(1, child_call_counter.count().await);

        // Dropping the child_call_counter should drop the weak pointer to it in hooks
        // as well.
        drop(child_call_counter);

        // Dispatching an event on child_hooks will not call out to child_call_counter
        // because it has been destroyed by the call to drop above.
        child_hooks.dispatch(&event).await.expect("Unable to call hooks.");

        // ChildCallCounter should be called before ParentCallCounter.
        assert_eq!(
            log(vec![
                "ChildCallCounter::OnAddDynamicChild",
                "ParentCallCounter::OnAddDynamicChild",
                "ParentCallCounter::OnAddDynamicChild",
            ]),
            event_log.get().await
        );
    }
}
